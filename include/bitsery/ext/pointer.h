//MIT License
//
//Copyright (c) 2017 Mindaugas Vinkelis
//
//Permission is hereby granted, free of charge, to any person obtaining a copy
//of this software and associated documentation files (the "Software"), to deal
//in the Software without restriction, including without limitation the rights
//to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//copies of the Software, and to permit persons to whom the Software is
//furnished to do so, subject to the following conditions:
//
//The above copyright notice and this permission notice shall be included in all
//copies or substantial portions of the Software.
//
//THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//SOFTWARE.

#ifndef BITSERY_EXT_POINTER_H
#define BITSERY_EXT_POINTER_H

#include <unordered_map>
#include <vector>
#include <memory>

namespace bitsery {

    namespace ext {

        //forward declare
        class PointerLinkingContext;

        namespace details_pointer {

            enum class PointerOwnershipType:uint8_t {
                //is not responsible for pointer lifetime management.
                Observer,
                //only ONE owner is responsible for this pointers creation/destruction
                Owner,
                //MANY shared owners is responsible for pointer creation/destruction
                //requires additional context to manage shared owners themselves.
                Shared
            };

            class PointerLinkingContextSerialization {
            public:
                explicit PointerLinkingContextSerialization()
                        :_currId{0},
                         _ptrMap{}
                {}

                PointerLinkingContextSerialization(const PointerLinkingContextSerialization&) = delete;
                PointerLinkingContextSerialization& operator = (const PointerLinkingContextSerialization&) = delete;

                PointerLinkingContextSerialization(PointerLinkingContextSerialization&&) = default;
                PointerLinkingContextSerialization& operator = (PointerLinkingContextSerialization&&) = default;
                ~PointerLinkingContextSerialization() = default;


                size_t createId(const void *ptr, PointerOwnershipType ptrType) {
                    if (ptr == nullptr)
                        return 0u;

                    auto res = _ptrMap.emplace(ptr, PointerInfo{_currId + 1u, ptrType});
                    auto& ptrInfo = res.first->second;
                    if (res.second) {
                        ++_currId;
                        return ptrInfo.id;
                    }
                    //ptr already exists
                    //for observer return success
                    if (ptrType == PointerOwnershipType::Observer)
                        return ptrInfo.id;
                    //set owner and return success
                    if (ptrInfo.ownershipType == PointerOwnershipType::Observer) {
                        ptrInfo.ownershipType = ptrType;
                        return ptrInfo.id;
                    }
                    //only shared ownership can get here multiple times
                    assert(ptrType == PointerOwnershipType::Shared);
                    return ptrInfo.id;
                }

                template <typename S, typename TPtr>
                void serialize(S& s, const TPtr& obj) {
                    using TNonPtr = typename std::remove_pointer<TPtr>::type;
                    static_assert(!std::is_polymorphic<TNonPtr>::value, "Polymorphic types are not supported");
                    serializeImpl(s, *obj, details::IsFundamentalType<TNonPtr>{});
                }

                //valid, when all pointers have owners.
                //we cannot serialize pointers, if we haven't serialized objects themselves
                bool isPointerSerializationValid() const {
                    return std::all_of(_ptrMap.begin(), _ptrMap.end(), [](const std::pair<const void*, PointerInfo>& p) {
                        return p.second.ownershipType != PointerOwnershipType::Observer;
                    });
                }

            private:
                template <typename S, typename T>
                void serializeImpl(S& s, const T& obj, std::true_type) {
                    s.template value<sizeof(T)>(obj);
                }

                template <typename S, typename T>
                void serializeImpl(S& s, const T& obj, std::false_type) {
                    s.object(obj);
                }

                struct PointerInfo {
                    PointerInfo(size_t id_, PointerOwnershipType ownershipType_)
                            :id{id_},
                             ownershipType{ownershipType_}
                    {};
                    size_t id;
                    PointerOwnershipType ownershipType;
                };

                size_t _currId;
                std::unordered_map<const void*, PointerInfo> _ptrMap;

            };

            //this class is used to store context for shared ptr owners
            struct SharedContextBase {
                virtual ~SharedContextBase() = default;
            };

            //helper functions that creates or destroys pointers
            //useful, because can be specialized
            template <typename T>
            void destroyPointer(T& ptr) {
                if (ptr) {
                    delete ptr;
                }
                ptr = nullptr;
            }

            template <typename T>
            void createPointer(T& ptr) {
                using TNonPtr = typename std::remove_pointer<T>::type;
                if (ptr == nullptr)
                    ptr = new TNonPtr{};
            }

            class PointerLinkingContextDeserialization {
            public:
                explicit PointerLinkingContextDeserialization()
                        : _idMap{}
                {}

                PointerLinkingContextDeserialization(const PointerLinkingContextDeserialization&) = delete;
                PointerLinkingContextDeserialization& operator = (const PointerLinkingContextDeserialization&) = delete;

                PointerLinkingContextDeserialization(PointerLinkingContextDeserialization&&) = default;
                PointerLinkingContextDeserialization& operator = (PointerLinkingContextDeserialization&&) = default;
                ~PointerLinkingContextDeserialization() = default;

                void processOwnerPtr(size_t id, void* ptr, PointerOwnershipType ptrType) {
                    assert(id != 0 && ptr != nullptr && ptrType != PointerOwnershipType::Observer);
                    auto res = _idMap.emplace(id, PointerInfo{id, ptr, ptrType});
                    auto& ptrInfo = res.first->second;
                    if (!res.second) {
                        assert(ptrInfo.ownershipType != PointerOwnershipType::Owner);
                        //if already exists, then process observer list
                        if (ptrInfo.ownershipType == PointerOwnershipType::Observer) {
                            ptrInfo.ptrAddress = ptr;
                            ptrInfo.ownershipType = ptrType;
                            processObserverList(ptrInfo.observersList, ptr);
                        }
                    }
                }

                void processObserverPtr(size_t id, void* (&ptr)) {
                    auto res = _idMap.emplace(id, PointerInfo{id, nullptr, PointerOwnershipType::Observer});
                    auto& ptrInfo = res.first->second;
                    if (ptrInfo.ptrAddress)
                        ptr = ptrInfo.ptrAddress;
                    else
                        ptrInfo.observersList.push_back(ptr);
                }

                template <typename S, typename TPtr>
                void deserialize(S& s, TPtr& obj) {
                    using TNonPtr = typename std::remove_pointer<TPtr>::type;
                    static_assert(!std::is_polymorphic<TNonPtr>::value, "Polymorphic types are not supported");
                    createPointer(obj);
                    deserializeImpl(s, *obj, details::IsFundamentalType<TNonPtr>{});
                }

                //valid, when all pointers has owners
                bool isPointerDeserializationValid() const {
                    return std::all_of(_idMap.begin(), _idMap.end(), [](const std::pair<const size_t, PointerInfo>& p) {
                        return p.second.ownershipType != PointerOwnershipType::Observer;
                    });
                }

            private:

                struct PointerInfo {
                    PointerInfo(size_t id_, void* ptr, PointerOwnershipType ownershipType_)
                            :id{id_},
                             ownershipType{ownershipType_},
                             ptrAddress{ptr},
                             observersList{},
                             sharedContext{}
                    {};
                    PointerInfo(const PointerInfo&) = delete;
                    PointerInfo& operator = (const PointerInfo&) = delete;
                    PointerInfo(PointerInfo&&) = default;
                    PointerInfo&operator = (PointerInfo&&) = default;
                    ~PointerInfo() = default;

                    size_t id;
                    PointerOwnershipType ownershipType;
                    void* ptrAddress;
                    std::vector<std::reference_wrapper<void*>> observersList;
                    std::unique_ptr<SharedContextBase> sharedContext;
                };

                void processObserverList(std::vector<std::reference_wrapper<void*>>& observers, void* ptr) {
                    for (auto& o:observers)
                        o.get() = ptr;
                    observers.clear();
                    observers.shrink_to_fit();
                }

                template <typename S, typename T>
                void deserializeImpl(S& s, T& obj, std::true_type) {
                    s.template value<sizeof(T)>(obj);
                }

                template <typename S, typename T>
                void deserializeImpl(S& s, T& obj, std::false_type) {
                    s.object(obj);
                }

                std::unordered_map<size_t, PointerInfo> _idMap;
            };

            template <typename S>
            PointerLinkingContext& getLinkingContext(S& s) {
                auto res = s.template context<PointerLinkingContext>();
                assert(res != nullptr);
                return *res;
            }
        }

        //this class is for convenience
        class PointerLinkingContext:
                public details_pointer::PointerLinkingContextSerialization,
                public details_pointer::PointerLinkingContextDeserialization {
        public:
            bool isValid() {
                return isPointerSerializationValid() && isPointerDeserializationValid();
            }
        };


        class PointerOwner {
        public:
            template<typename Ser, typename Writer, typename T, typename Fnc>
            void serialize(Ser &ser, Writer &w, const T &obj, Fnc &&) const {
                auto& ctx = details_pointer::getLinkingContext(ser);
                auto id = ctx.createId(obj, details_pointer::PointerOwnershipType::Owner);
                details::writeSize(w, id);
                if (id)
                    ctx.serialize(ser, obj);
            }

            template<typename Des, typename Reader, typename T, typename Fnc>
            void deserialize(Des &des, Reader &r, T &obj, Fnc &&) const {
                details_pointer::getLinkingContext(des);
                size_t id{};
                details::readSize(r, id, std::numeric_limits<size_t>::max());
                if (id) {
                    auto& ctx = details_pointer::getLinkingContext(des);
                    ctx.deserialize(des, obj);
                    ctx.processOwnerPtr(id, obj, details_pointer::PointerOwnershipType::Owner);
                } else {
                    details_pointer::destroyPointer(obj);
                }
            }
        };

        class PointerObserver {
        public:

            template<typename Ser, typename Writer, typename T, typename Fnc>
            void serialize(Ser &ser, Writer &w, const T &obj, Fnc &&) const {
                auto& ctx = details_pointer::getLinkingContext(ser);
                details::writeSize(w, ctx.createId(obj, details_pointer::PointerOwnershipType::Observer));
            }

            template<typename Des, typename Reader, typename T, typename Fnc>
            void deserialize(Des &des, Reader &r, T &obj, Fnc &&) const {
                size_t id{};
                details::readSize(r, id, std::numeric_limits<size_t>::max());
                if (id) {
                    auto& ctx = details_pointer::getLinkingContext(des);
                    ctx.processObserverPtr(id, reinterpret_cast<void*&>(obj));
                } else {
                    obj = nullptr;
                }
            }

        };

        class ReferencedByPointer {
        public:
            template<typename Ser, typename Writer, typename T, typename Fnc>
            void serialize(Ser &ser, Writer &w, const T &obj, Fnc && fnc) const {
                auto& ctx = details_pointer::getLinkingContext(ser);
                details::writeSize(w, ctx.createId(&obj, details_pointer::PointerOwnershipType::Owner));
                fnc(const_cast<T&>(obj));
            }

            template<typename Des, typename Reader, typename T, typename Fnc>
            void deserialize(Des &des, Reader &r, T &obj, Fnc && fnc) const {
                size_t id{};
                details::readSize(r, id, std::numeric_limits<size_t>::max());
                if (id) {
                    auto& ctx = details_pointer::getLinkingContext(des);
                    fnc(obj);
                    ctx.processOwnerPtr(id, &obj, details_pointer::PointerOwnershipType::Owner);
                } else {
                    //cannot be null for references
                    r.setError(ReaderError::InvalidPointer);
                }
            }
        };

    }

    namespace traits {

        template<typename T>
        struct ExtensionTraits<ext::PointerOwner, T*> {
            using TValue = T;
            static constexpr bool SupportValueOverload = true;
            static constexpr bool SupportObjectOverload = true;
            //pointers cannot have lamba overload, when polymorphism support will be added
            static constexpr bool SupportLambdaOverload = false;
        };

        template<typename T>
        struct ExtensionTraits<ext::PointerObserver, T*> {
            //although pointer observer doesn't serialize anything, but we still add value overload support to be consistent with pointer owners
            using TValue = T;
            static constexpr bool SupportValueOverload = true;
            static constexpr bool SupportObjectOverload = true;
            //pointers cannot have lamba overload, when polymorphism support will be added
            static constexpr bool SupportLambdaOverload = false;
        };

        template<typename T>
        struct ExtensionTraits<ext::ReferencedByPointer, T> {
            //allow everything, because it is serialized as regular type, except it also creates pointerId that is required by NonOwningPointer to work
            using TValue = T;
            static constexpr bool SupportValueOverload = true;
            static constexpr bool SupportObjectOverload = true;
            static constexpr bool SupportLambdaOverload = true;
        };
    }

}


#endif //BITSERY_EXT_POINTER_H
