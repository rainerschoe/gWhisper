// Copyright 2019 IBM Corporation
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <libCli/GrammarConstruction.hpp>
#include <third_party/gRPC_utils/proto_reflection_descriptor_database.h>

#include <libCli/cliUtils.hpp>

using namespace ArgParse;

namespace cli
{

class GrammarInjectorMethodArgs : public GrammarInjector
{
    public:
        GrammarInjectorMethodArgs(Grammar & f_grammar, const std::string & f_elementName = "") :
            GrammarInjector("MethodArgs", f_elementName),
            m_grammar(f_grammar)
        {
        }

        virtual ~GrammarInjectorMethodArgs()
        {
        }

        virtual GrammarElement * getGrammar(ParsedElement * f_parseTree) override
        {
            // FIXME: we are already completing this without a service parsed.
            //  this works in most cases, as it will just fail. however this is not really a nice thing.
            std::string serverAddress = f_parseTree->findFirstChild("ServerAddress");
            std::string serverPort = f_parseTree->findFirstChild("ServerPort");
            std::string serviceName = f_parseTree->findFirstChild("Service");
            std::string methodName = f_parseTree->findFirstChild("Method");

            //std::cout << f_parseTree->getDebugString() << std::endl;
            //std::cout << "Injecting grammar for " << serverAddress << ":" << serverPort << " " << serviceName << " " << methodName << std::endl;
            if(serverPort == "")
            {
                serverPort = "50051";
            }
            serverAddress += ":" + serverPort;
            //std::cout << "Server addr: " << serverAddress << std::endl;
            std::shared_ptr<grpc::Channel> channel =
                grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());

            if(not waitForChannelConnected(channel, getConnectTimeoutMs(f_parseTree)))
            {
                return nullptr;
            }

            grpc::ProtoReflectionDescriptorDatabase descDb(channel);
            grpc::protobuf::DescriptorPool descPool(&descDb);

            const grpc::protobuf::ServiceDescriptor* service = descPool.FindServiceByName(serviceName);
            if(service == nullptr)
            {
                //std::cerr << "Error: Service not found" << std::endl;
                return nullptr;
            }

            auto method = service->FindMethodByName(methodName);
            if(method == nullptr)
            {
                //std::cerr << "Error: Method not found" << std::endl;
                return nullptr;
            }

            if(method->client_streaming())
            {
                std::cerr << "Error: Client streaming RPCs not supported." << std::endl;
                return nullptr;
            }

            auto concat = m_grammar.createElement<Concatenation>();

            auto separation = m_grammar.createElement<WhiteSpace>();
            //auto separation = m_grammar.createElement<Alternation>();
            //separation->addChild(m_grammar.createElement<WhiteSpace>());
            //separation->addChild(m_grammar.createElement<FixedString>(","));
            concat->addChild(separation);

            auto fields = getMessageGrammar(method->input_type());
            concat->addChild(fields);

            auto result = m_grammar.createElement<Repetition>("Fields");
            result->addChild(concat);

            return result;
        };

    private:

        void addFieldValueGrammar(GrammarElement * f_fieldGrammar, const grpc::protobuf::FieldDescriptor * f_field)
        {
            switch(f_field->cpp_type())
            {
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_FLOAT:
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_DOUBLE:
                    // TODO: make regex match closer
                    f_fieldGrammar->addChild(m_grammar.createElement<RegEx>("[\\+-\\.pP0-9a-fA-F]+", "FieldValue"));
                    break;
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_INT32:
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_INT64:
                    f_fieldGrammar->addChild(m_grammar.createElement<RegEx>("[\\+-]?(0x|0b)?[0-9a-fA-F]+", "FieldValue"));
                    break;
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_UINT32:
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_UINT64:
                    f_fieldGrammar->addChild(m_grammar.createElement<RegEx>("\\+?(0x|0b)?[0-9a-fA-F]+", "FieldValue"));
                    break;
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_BOOL:
                    {
                        auto boolGrammar = m_grammar.createElement<Alternation>("FieldValue");
                        boolGrammar->addChild(m_grammar.createElement<FixedString>("true"));
                        boolGrammar->addChild(m_grammar.createElement<FixedString>("false"));
                        boolGrammar->addChild(m_grammar.createElement<FixedString>("1"));
                        boolGrammar->addChild(m_grammar.createElement<FixedString>("0"));
                        f_fieldGrammar->addChild(boolGrammar);
                        break;
                    }
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_ENUM:
                    {
                        const google::protobuf::EnumDescriptor * enumDesc = f_field->enum_type();
                        auto enumGrammar = m_grammar.createElement<Alternation>("FieldValue");
                        for(int i = 0; i<enumDesc->value_count(); i++)
                        {
                            const google::protobuf::EnumValueDescriptor * enumValueDesc = enumDesc->value(i);
                            // FIXME: null possible?
                            enumGrammar->addChild(m_grammar.createElement<FixedString>(enumValueDesc->name()));
                        }
                        f_fieldGrammar->addChild(enumGrammar);
                        break;
                    }
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_STRING:
                    if(f_field->type() == grpc::protobuf::FieldDescriptor::Type::TYPE_BYTES)
                    {
                        auto bytesContainer = m_grammar.createElement<Concatenation>("FieldValue");
                        bytesContainer->addChild(m_grammar.createElement<FixedString>("0x"));
                        bytesContainer->addChild(m_grammar.createElement<RegEx>("[0-9a-fA-F]*", ""));
                        f_fieldGrammar->addChild(bytesContainer);
                    }
                    else
                    {
                        // FIXME: commented solution does not work:
                        //        we always complete "::" as empty string matches
                        //        Solution would be to change the parser to never
                        //        attempt completion when a regex is currently
                        //        parsed. This requires changes in the parser
                        //        and could not be implemented on short notice.
                        //auto stringContainer = m_grammar.createElement<Concatenation>("StringContainer");
                        //stringContainer->addChild(m_grammar.createElement<FixedString>(":"));
                        //stringContainer->addChild(m_grammar.createElement<RegEx>("[^:]*", "FieldValue"));
                        //stringContainer->addChild(m_grammar.createElement<FixedString>(":"));
                        //f_fieldGrammar->addChild(stringContainer);

                        // Using this as a workaround until parser gets better regex support
                        f_fieldGrammar->addChild(m_grammar.createElement<RegEx>("[^ ]*", "FieldValue"));
                    }
                    break;
                case grpc::protobuf::FieldDescriptor::CppType::CPPTYPE_MESSAGE:
                    {
                        //std::cerr << "Field '" << field->name() << "' has message type: '" << field->type_name() << "'" << std::endl;
                        auto subMessage = m_grammar.createElement<Concatenation>("FieldValue");
                        subMessage->addChild(m_grammar.createElement<FixedString>(":"));

                        auto childFieldsRep = m_grammar.createElement<Repetition>("Fields");
                        auto concat = m_grammar.createElement<Concatenation>();
                        auto fieldsAlt = getMessageGrammar(f_field->message_type());
                        concat->addChild(fieldsAlt);

                        auto separation = m_grammar.createElement<WhiteSpace>();
                        //auto separation = m_grammar.createElement<Alternation>();
                        //separation->addChild(m_grammar.createElement<WhiteSpace>());
                        //separation->addChild(m_grammar.createElement<FixedString>(","));
                        concat->addChild(separation);

                        childFieldsRep->addChild(concat);
                        subMessage->addChild(childFieldsRep);

                        subMessage->addChild(m_grammar.createElement<FixedString>(":"));
                        f_fieldGrammar->addChild(subMessage);
                        break;
                    }
                default:
                    std::cerr << "Field '" << f_field->name() << "' has unsupported type: '" << f_field->type_name() << "'" << std::endl;
                    break;
            }
        }

        GrammarElement * getMessageGrammar(const grpc::protobuf::Descriptor* f_messageDescriptor)
        {
            auto fields = m_grammar.createElement<Alternation>();
            // iterate over fields:
            for(int i = 0; i< f_messageDescriptor->field_count(); i++)
            {
                const grpc::protobuf::FieldDescriptor * field = f_messageDescriptor->field(i);

                //std::cerr << "Iterating field " << std::to_string(i) << " of message " << f_messageDescriptor->name() << "with name: '" << field->name() <<"'"<< std::endl;

                // now we add grammar to the fields alternation:
                auto fieldGrammar = m_grammar.createElement<Concatenation>();
                fieldGrammar->addChild(m_grammar.createElement<FixedString>(field->name(), "FieldName"));
                fieldGrammar->addChild(m_grammar.createElement<FixedString>("="));
                fields->addChild(fieldGrammar);
                if(field->is_repeated())
                {
                    auto repeatedValue = m_grammar.createElement<Concatenation>("RepeatedValue");
                    addFieldValueGrammar(repeatedValue, field);

                    auto repeatedGrammar = m_grammar.createElement<Concatenation>("FieldValue");
                    repeatedGrammar->addChild(m_grammar.createElement<FixedString>(":"));

                    repeatedGrammar->addChild(repeatedValue);

                    auto repeatedOptionalEntry = m_grammar.createElement<Concatenation>();
                    repeatedOptionalEntry->addChild(m_grammar.createElement<FixedString>(","));
                    repeatedOptionalEntry->addChild(m_grammar.createElement<WhiteSpace>());
                    repeatedOptionalEntry->addChild(repeatedValue);

                    auto repeatedOptionalValues = m_grammar.createElement<Repetition>();
                    repeatedOptionalValues->addChild(repeatedOptionalEntry);
                    repeatedGrammar->addChild(repeatedOptionalValues);
                    repeatedGrammar->addChild(m_grammar.createElement<FixedString>(":"));


                    fieldGrammar->addChild(repeatedGrammar);
                }
                else
                {
                    // the simple case:
                    addFieldValueGrammar(fieldGrammar, field);
                }
            }

            //std::cout << "Grammar generated:\n" << fields->toString() << std::endl;
            return fields;
        }



        Grammar & m_grammar;

};

class GrammarInjectorMethods : public GrammarInjector
{
    public:
        GrammarInjectorMethods(Grammar & f_grammar, const std::string & f_elementName = "") :
            GrammarInjector("Method", f_elementName),
            m_grammar(f_grammar)
        {
        }

        virtual GrammarElement * getGrammar(ParsedElement * f_parseTree) override
        {
            // FIXME: we are already completing this without a service parsed.
            //  this works in most cases, as it will just fail. however this is not really a nice thing.
            std::string serverAddress = f_parseTree->findFirstChild("ServerAddress");
            std::string serverPort = f_parseTree->findFirstChild("ServerPort");
            std::string serviceName = f_parseTree->findFirstChild("Service");

            //std::cout << f_parseTree->getDebugString() << std::endl;
            //std::cout << "Injecting grammar for " << serverAddress << ":" << serverPort << " " << serviceName << std::endl;
            if(serverPort == "")
            {
                serverPort = "50051";
            }
            serverAddress += ":" + serverPort;
            //std::cout << "Server addr: " << serverAddress << std::endl;
            std::shared_ptr<grpc::Channel> channel =
                grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());


            if(not waitForChannelConnected(channel, getConnectTimeoutMs(f_parseTree)))
            {
                return nullptr;
            }

            grpc::ProtoReflectionDescriptorDatabase descDb(channel);
            grpc::protobuf::DescriptorPool descPool(&descDb);

            const grpc::protobuf::ServiceDescriptor* service = descPool.FindServiceByName(serviceName);

            auto result = m_grammar.createElement<Alternation>();
            if(service != nullptr)
            {
                for (int i = 0; i < service->method_count(); ++i)
                {
                    result->addChild(m_grammar.createElement<FixedString>(service->method(i)->name()));
                }
            }
            else
            {
                return nullptr;
            }
            return result;
        };

    private:
        Grammar & m_grammar;

};

class GrammarInjectorServices : public GrammarInjector
{
    public:
        GrammarInjectorServices(Grammar & f_grammar, const std::string & f_elementName = "") :
            GrammarInjector("Service", f_elementName),
            m_grammar(f_grammar)
        {
        }

        virtual ~GrammarInjectorServices()
        {
        }

        virtual GrammarElement * getGrammar(ParsedElement * f_parseTree) override
        {
            std::string serverAddress = f_parseTree->findFirstChild("ServerAddress");
            std::string serverPort = f_parseTree->findFirstChild("ServerPort");
            if(serverPort == "")
            {
                serverPort = "50051";
            }
            serverAddress += ":" + serverPort;
            //std::cout << "Server addr: " << serverAddress << std::endl;
            std::shared_ptr<grpc::Channel> channel =
                grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());


            if(not waitForChannelConnected(channel, getConnectTimeoutMs(f_parseTree)))
            {
                return nullptr;
            }

            grpc::ProtoReflectionDescriptorDatabase descDb(channel);

            std::vector<grpc::string> serviceList;
            if(not descDb.GetServices(&serviceList) )
            {
                printf("error retrieving service list\n");
                return nullptr;
            }

            auto result = m_grammar.createElement<Alternation>();
            for(auto service : serviceList)
            {

                result->addChild(m_grammar.createElement<FixedString>(service));
            }
            return result;
        };

    private:
        Grammar & m_grammar;

};

GrammarElement * constructGrammar(Grammar & f_grammarPool)
{
    // user defined output formatting
    // something like this will match: @.fru_info_list:found fru in slot /slot_id/:
    GrammarElement * formatTargetSpecifier = f_grammarPool.createElement<Concatenation>("CustomOutputFormat");
    formatTargetSpecifier->addChild(f_grammarPool.createElement<FixedString>("@"));
    GrammarElement * formatTargetTree = f_grammarPool.createElement<Repetition>("TargetSpecifier");
    GrammarElement * formatTargetPart = f_grammarPool.createElement<Concatenation>();
    formatTargetPart->addChild(f_grammarPool.createElement<FixedString>("."));
    formatTargetPart->addChild(f_grammarPool.createElement<RegEx>("[^:]*", "PartialTarget"));
    formatTargetTree->addChild(formatTargetPart);
    formatTargetSpecifier->addChild(formatTargetTree);
    formatTargetSpecifier->addChild(f_grammarPool.createElement<FixedString>(":"));
    GrammarElement * formatOutputSpecifier = f_grammarPool.createElement<Repetition>("OutputFormatString");
    GrammarElement * formatOutputSpecifierAlternation = f_grammarPool.createElement<Alternation>();
    formatOutputSpecifierAlternation->addChild(f_grammarPool.createElement<RegEx>("[^:/]+", "OutputFixedString")); // a real string
    GrammarElement * fieldReference = f_grammarPool.createElement<Concatenation>();
    fieldReference->addChild(f_grammarPool.createElement<FixedString>("/"));
    fieldReference->addChild(f_grammarPool.createElement<RegEx>("[^/,]*", "OutputFieldReference")); // field reference
    GrammarElement * modifiers = f_grammarPool.createElement<Repetition>("OutputFormatModifiers");
    GrammarElement * modifierConcat = f_grammarPool.createElement<Concatenation>();
    modifierConcat->addChild(f_grammarPool.createElement<FixedString>(","));
    GrammarElement * modifierAlternation = f_grammarPool.createElement<Alternation>("Modifier");
    modifierAlternation->addChild(f_grammarPool.createElement<FixedString>("hex"));
    modifierAlternation->addChild(f_grammarPool.createElement<FixedString>("dec"));
    modifierAlternation->addChild(f_grammarPool.createElement<FixedString>("zeroPadding"));
    modifierAlternation->addChild(f_grammarPool.createElement<FixedString>("spacePadding"));
    modifierAlternation->addChild(f_grammarPool.createElement<FixedString>("noPadding"));
    modifiers->addChild(modifierConcat);
    modifierConcat->addChild(modifierAlternation);
    fieldReference->addChild(modifiers);
    fieldReference->addChild(f_grammarPool.createElement<FixedString>("/"));
    formatOutputSpecifierAlternation->addChild(fieldReference);
    formatOutputSpecifier->addChild(formatOutputSpecifierAlternation);

    formatTargetSpecifier->addChild(formatOutputSpecifier);
    formatTargetSpecifier->addChild(f_grammarPool.createElement<FixedString>(":"));

    GrammarElement * customOutputFormat = f_grammarPool.createElement<Concatenation>();
    customOutputFormat->addChild(f_grammarPool.createElement<FixedString>("--customOutput"));
    customOutputFormat->addChild(f_grammarPool.createElement<WhiteSpace>());
    customOutputFormat->addChild(formatTargetSpecifier);
    // TODO add this to options

    // options
    GrammarElement * options = f_grammarPool.createElement<Repetition>(); // TODO: support multiple options
    GrammarElement * optionsconcat = f_grammarPool.createElement<Concatenation>();
    GrammarElement * optionsalt = f_grammarPool.createElement<Alternation>();
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("-h", "Help"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--help", "Help"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--complete", "Complete"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--debugComplete", "CompleteDebug"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--dot", "DotExport"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--noColor", "NoColor"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--color", "Color"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--version", "Version"));
    optionsalt->addChild(f_grammarPool.createElement<FixedString>("--printParsedMessage", "PrintParsedMessage"));
    GrammarElement * timeoutOption = f_grammarPool.createElement<Concatenation>();
    timeoutOption->addChild(f_grammarPool.createElement<FixedString>("--connectTimeoutMilliseconds="));
    timeoutOption->addChild(f_grammarPool.createElement<RegEx>("[0-9]+", "connectTimeout"));
    optionsalt->addChild(timeoutOption);
    optionsalt->addChild(customOutputFormat);
    // FIXME FIXME FIXME: we cannot distinguish between --complete and --completeDebug.. this is a problem for arguments too, as we cannot guarantee, that we do not have an argument starting with the name of an other argument.
    // -> could solve by makeing FixedString greedy
    optionsconcat->addChild(optionsalt);
    optionsconcat->addChild(
                f_grammarPool.createElement<WhiteSpace>()
            );
    //optionsconcat->addChild(
    //        f_grammarPool.createElement<Optional>()->addChild(
    //            f_grammarPool.createElement<WhiteSpace>()
    //            )
    //        );
    options->addChild(optionsconcat);

    // Server address
    GrammarElement * serverAddress = f_grammarPool.createElement<Alternation>("ServerAddress");
    serverAddress->addChild(f_grammarPool.createElement<RegEx>("\\d+\\.\\d+\\.\\d+\\.\\d+", "IPv4Address"));
    serverAddress->addChild(f_grammarPool.createElement<RegEx>("\\[?[0-9a-fA-F:]+\\]?", "IPv6Address"));
    serverAddress->addChild(f_grammarPool.createElement<RegEx>("[^\\.:\\[\\] ]+", "Hostname"));

    // Server port
    GrammarElement * cServerPort = f_grammarPool.createElement<Concatenation>();
    cServerPort->addChild(f_grammarPool.createElement<FixedString>(":"));
    cServerPort->addChild(f_grammarPool.createElement<RegEx>("\\d+", "ServerPort"));
    GrammarElement * serverPort = f_grammarPool.createElement<Optional>();
    serverPort->addChild(cServerPort);

    //GrammarElement * testAlt = f_grammarPool.createElement<Alternation>("TestAlt");
    //testAlt->addChild(f_grammarPool.createElement<FixedString>("challo"));
    //testAlt->addChild(f_grammarPool.createElement<FixedString>("ctschuess"));

    // main concat:
    GrammarElement * cmain = f_grammarPool.createElement<Concatenation>();
    cmain->addChild(options);
    cmain->addChild(serverAddress);
    cmain->addChild(serverPort);
    //cmain->addChild(testAlt);
    //cmain->addChild(f_grammarPool.createElement<RegEx>(std::regex("\\S+"), "ServerAddress"));
    cmain->addChild(f_grammarPool.createElement<WhiteSpace>());
    cmain->addChild(f_grammarPool.createElement<GrammarInjectorServices>(f_grammarPool, "Service"));
    cmain->addChild(f_grammarPool.createElement<WhiteSpace>());
    cmain->addChild(f_grammarPool.createElement<GrammarInjectorMethods>(f_grammarPool, "Method"));
    //cmain->addChild(f_grammarPool.createElement<WhiteSpace>());
    cmain->addChild(f_grammarPool.createElement<GrammarInjectorMethodArgs>(f_grammarPool, "MethodArgs"));

    return cmain;
}
}
